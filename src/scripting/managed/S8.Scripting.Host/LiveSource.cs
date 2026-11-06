using System.Collections.Immutable;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Emit;
using S8.Scripting;

namespace S8.Scripting.Host;

// Development mode for a JIT mod whose folder holds a source\ directory. The host
// compiles those files itself (Debug) so it owns the edit-and-continue baseline,
// then applies changed method bodies to the running assembly in place. Anything
// structural (added or removed members, field or signature changes) or refused by
// Roslyn or the runtime is left for a full rebuild and module swap instead.
internal sealed class LiveSource
{
    public const string Folder = "source";

    private static readonly Guid SlotMap = new("755F52A8-91C5-45BE-B4B8-209571E552BD");
    private static readonly Guid LambdaMap = new("A643004C-0240-496F-A783-30D64F4979DE");
    private static readonly Guid StateMachineMap = new("8B78CD68-2EDE-420B-980B-E15884B8AAA3");
    private static readonly string[] ImplicitUsings =
        ["System", "System.Collections.Generic", "System.IO", "System.Linq", "System.Net.Http", "System.Threading", "System.Threading.Tasks"];

    private readonly string directory, assemblyName;
    private readonly long intervalMilliseconds;
    private CSharpCompilation compilation;
    private EmitBaseline? baseline;
    private PEReader? image;
    private string snapshot;
    private long lastCheck;

    private LiveSource(string directory, string assemblyName, long intervalMilliseconds)
    {
        this.directory = directory;
        this.assemblyName = assemblyName;
        this.intervalMilliseconds = intervalMilliseconds;
        snapshot = Snapshot();
        compilation = Compile(Parse());
        Image = Rebuild();
    }

    // The current full image: what a fresh load or a swap must use.
    public byte[] Image { get; private set; }

    public static LiveSource? Find(string modDirectory, string assemblyPath, long intervalMilliseconds = 500)
    {
        string directory = Path.Combine(modDirectory, Folder);
        return Directory.Exists(directory)
            ? new LiveSource(directory, Path.GetFileNameWithoutExtension(assemblyPath), intervalMilliseconds)
            : null;
    }

    // Cheap, throttled file check; a set that does not compile is reported once
    // and the running code is kept.
    public bool HasChanges()
    {
        long now = Environment.TickCount64;
        if (now - lastCheck < intervalMilliseconds)
            return false;
        lastCheck = now;

        string current = Snapshot();
        if (current == snapshot)
            return false;
        snapshot = current;
        return true;
    }

    // Patches the loaded assembly. False means a swap is needed (reason says why);
    // the new sources then become the compilation that the swap will load.
    public bool TryPatch(Assembly loaded, out string reason)
    {
        CSharpCompilation next;
        try
        {
            next = Compile(Parse());
        }
        catch (InvalidDataException error)
        {
            reason = error.Message;
            return true; // Keep running the last good code until the sources compile.
        }

        var edits = MethodEdits(compilation, next, out reason);
        if (edits == null || !System.Reflection.Metadata.MetadataUpdater.IsSupported || baseline == null)
        {
            if (edits != null)
                reason = "이 실행 환경은 코드 차이분 적용을 지원하지 않습니다.";
            compilation = next;
            return false;
        }
        if (edits.Count == 0)
        {
            compilation = next;
            reason = "바뀐 메서드가 없습니다.";
            return true;
        }

        using var metadata = new MemoryStream();
        using var il = new MemoryStream();
        using var pdb = new MemoryStream();
        var result = next.EmitDifference(baseline, edits, _ => false, metadata, il, pdb);
        compilation = next;
        if (!result.Success)
        {
            reason = string.Join(Environment.NewLine, result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));
            return false;
        }

        try
        {
            System.Reflection.Metadata.MetadataUpdater.ApplyUpdate(loaded, metadata.ToArray(), il.ToArray(), pdb.ToArray());
        }
        catch (Exception error) when (error is InvalidOperationException or NotSupportedException)
        {
            reason = error.Message;
            return false;
        }

        baseline = result.Baseline;
        reason = $"메서드 {edits.Count}개를 그 자리에서 바꿨습니다.";
        return true;
    }

    // Starts a new generation from the current compilation (used by a swap).
    public byte[] Rebuild()
    {
        using var pe = new MemoryStream();
        using var pdb = new MemoryStream();
        var result = compilation.Emit(pe, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
        if (!result.Success)
            throw new InvalidDataException(Errors(result.Diagnostics));

        byte[] bytes = pe.ToArray();
        image?.Dispose();
        image = new PEReader(ImmutableArray.Create(bytes));
        baseline = EmitBaseline.CreateInitialBaseline(compilation, ModuleMetadata.CreateFromImage(bytes),
            DebugInformation(pdb.ToArray()), LocalSignature, hasPortableDebugInformation: true);
        Image = bytes;
        return bytes;
    }

    private StandaloneSignatureHandle LocalSignature(MethodDefinitionHandle handle)
    {
        var current = image!;
        int address = current.GetMetadataReader().GetMethodDefinition(handle).RelativeVirtualAddress;
        return address == 0 ? default : current.GetMethodBody(address).LocalSignature;
    }

    private static Func<MethodDefinitionHandle, EditAndContinueMethodDebugInformation> DebugInformation(byte[] pdb)
    {
        var reader = MetadataReaderProvider.FromPortablePdbImage(ImmutableArray.Create(pdb)).GetMetadataReader();
        return handle =>
        {
            ImmutableArray<byte> slots = default, lambdas = default, states = default;
            foreach (var entry in reader.GetCustomDebugInformation(handle))
            {
                var information = reader.GetCustomDebugInformation(entry);
                var kind = reader.GetGuid(information.Kind);
                var value = reader.GetBlobContent(information.Value);
                if (kind == SlotMap)
                    slots = value;
                else if (kind == LambdaMap)
                    lambdas = value;
                else if (kind == StateMachineMap)
                    states = value;
            }
            return EditAndContinueMethodDebugInformation.Create(slots, lambdas, states);
        };
    }

    // Update edits for changed method bodies; null when the change is structural.
    private static List<SemanticEdit>? MethodEdits(CSharpCompilation before, CSharpCompilation after, out string reason)
    {
        var old = Members(before);
        var current = Members(after);
        reason = "";

        if (!old.Keys.ToHashSet().SetEquals(current.Keys))
        {
            reason = "형식이나 멤버가 추가·삭제되어 모듈을 교체합니다.";
            return null;
        }

        var edits = new List<SemanticEdit>();
        foreach (var (id, symbol) in current)
        {
            var previous = old[id];
            // Implicit members follow their declarations (field initializers are fields).
            if (symbol.IsImplicitlyDeclared || Declaration(previous) == Declaration(symbol))
                continue;
            if (symbol is IMethodSymbol)
            {
                edits.Add(new SemanticEdit(SemanticEditKind.Update, previous, symbol));
                continue;
            }
            if (symbol is IPropertySymbol or IEventSymbol or INamedTypeSymbol && !Initializer(previous, symbol))
                continue; // Their accessor or member bodies are compared on their own.

            reason = $"{symbol.ToDisplayString()} 선언이 바뀌어 모듈을 교체합니다.";
            return null;
        }
        return edits;
    }

    // Accessor and member bodies are compared separately; only an initializer
    // change makes a property structural.
    private static bool Initializer(ISymbol previous, ISymbol symbol)
    {
        static string Text(ISymbol value) => string.Concat(value.DeclaringSyntaxReferences
            .Select(reference => reference.GetSyntax()).OfType<PropertyDeclarationSyntax>()
            .Select(property => property.Initializer?.ToString()));
        return Text(previous) != Text(symbol);
    }

    private static string Declaration(ISymbol symbol) =>
        string.Join("\n", symbol.DeclaringSyntaxReferences.Select(reference => reference.GetSyntax().ToString()));

    private static Dictionary<string, ISymbol> Members(CSharpCompilation compilation)
    {
        var members = new Dictionary<string, ISymbol>();
        void Visit(INamespaceOrTypeSymbol container)
        {
            foreach (var member in container.GetMembers())
            {
                if (member is INamespaceSymbol space)
                {
                    Visit(space);
                    continue;
                }
                if (!member.Locations.Any(location => location.IsInSource))
                    continue;
                // Implicit members have no id of their own; key them by owner and name.
                string id = member.GetDocumentationCommentId() ?? member.ContainingSymbol + "::" + member.MetadataName;
                members[id] = member;
                if (member is INamedTypeSymbol type)
                    Visit(type);
            }
        }
        Visit(compilation.Assembly.GlobalNamespace);
        return members;
    }

    private List<SyntaxTree> Parse()
    {
        var options = new CSharpParseOptions(LanguageVersion.Latest);
        var trees = Directory.EnumerateFiles(directory, "*.cs", SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase)
            .Select(path => CSharpSyntaxTree.ParseText(
                Microsoft.CodeAnalysis.Text.SourceText.From(File.ReadAllText(path), Encoding.UTF8), options, path))
            .ToList();
        trees.Add(CSharpSyntaxTree.ParseText(
            Microsoft.CodeAnalysis.Text.SourceText.From(string.Concat(ImplicitUsings.Select(name => $"global using global::{name};\n")), Encoding.UTF8),
            options, Path.Combine(directory, "ImplicitUsings.g.cs")));
        return trees;
    }

    private CSharpCompilation Compile(List<SyntaxTree> trees)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Where(path => !Path.GetFileName(path).StartsWith("Microsoft.CodeAnalysis", StringComparison.OrdinalIgnoreCase))
            .Append(typeof(IScriptModule).Assembly.Location)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(path => MetadataReference.CreateFromFile(path));
        var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
            optimizationLevel: OptimizationLevel.Debug, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Enable);
        var result = CSharpCompilation.Create(assemblyName, trees, references, options);
        var errors = result.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).ToList();
        if (errors.Count != 0)
            throw new InvalidDataException(Errors(errors));
        return result;
    }

    private static string Errors(IEnumerable<Diagnostic> diagnostics) =>
        "모드 소스 컴파일 실패:" + Environment.NewLine + string.Join(Environment.NewLine, diagnostics.Take(20));

    private string Snapshot() =>
        string.Join("|", Directory.EnumerateFiles(directory, "*.cs", SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase)
            .Select(path => new FileInfo(path)).Select(file => $"{file.FullName}:{file.LastWriteTimeUtc.Ticks}:{file.Length}"));
}
