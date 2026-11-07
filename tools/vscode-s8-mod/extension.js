const fs = require('node:fs/promises');
const path = require('node:path');
const vscode = require('vscode');

function activate(context) {
    context.subscriptions.push(vscode.commands.registerCommand('s8ModTools.createMod', async () => {
        try {
            const folders = await vscode.window.showOpenDialog({
                canSelectFiles: false,
                canSelectFolders: true,
                openLabel: '게임 폴더 선택',
                title: 'SAN8RPK.exe가 있는 폴더를 선택하세요',
            });
            if (!folders)
                return;

            const gameDirectory = folders[0].fsPath;
            const requiredFiles = ['SAN8RPK.exe', 'scripts/S8.Scripting.dll', 'scripts/S8.Scripting.Host.dll'];
            for (const file of requiredFiles) {
                const stat = await fs.stat(path.join(gameDirectory, file), { throwIfNoEntry: false });
                if (!stat?.isFile())
                    throw new Error(`게임 폴더에 ${file} 파일이 없습니다.`);
            }

            const name = (await vscode.window.showInputBox({
                title: '게임에 표시할 모드 이름',
                validateInput: value => value.trim().length > 0 && value.trim().length <= 64
                    ? undefined : '1~64자로 입력하세요.',
            }))?.trim();
            if (!name)
                return;

            const id = await vscode.window.showInputBox({
                title: '모드 ID',
                placeHolder: 'my.hello',
                validateInput: value => /^[a-z][a-z0-9-]*(\.[a-z0-9-]+)+$/.test(value) && value.length <= 64
                    ? undefined : 'my.hello처럼 소문자로 시작하고 점으로 구분하세요.',
            });
            if (!id)
                return;

            const modDirectory = path.join(gameDirectory, 'scripts', 'mods', id);
            const manifest = {
                id, name, version: '0.1.0', apiVersion: 21,
                runtime: 'jit', entry: `${id}.dll`, type: 'S8.Generated.Mod',
            };
            const project = `<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="S8.Scripting">
      <HintPath>../../S8.Scripting.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
`;
            const source = `using S8.Scripting;

namespace S8.Generated;

public sealed class Mod : ScriptMod
{
    private UiScene? scene;

    protected override void OnLoad() =>
        AddMenu(MenuSurface.TitleEdit, ${JSON.stringify(name)}, Open);

    private void Open()
    {
        scene = new UiScene();
        scene.Create("Hello").Show(DialogKind.Message, ${JSON.stringify(name)},
            [new UiRow(0, "확인", "안녕하세요.")]);
    }

    protected override void OnUiClosed()
    {
        scene?.Dispose();
        scene = null;
    }
}
`;
            await fs.mkdir(path.dirname(modDirectory), { recursive: true });
            try {
                await fs.mkdir(modDirectory);
            } catch (error) {
                if (error.code === 'EEXIST')
                    throw new Error(`이미 있는 모드 폴더입니다. 다른 ID를 사용하세요. (${id})`);
                throw error;
            }
            const sourceDirectory = path.join(modDirectory, 'source');
            await fs.mkdir(sourceDirectory);
            const sourcePath = path.join(sourceDirectory, 'Mod.cs');
            await Promise.all([
                fs.writeFile(sourcePath, source),
                fs.writeFile(path.join(modDirectory, `${id}.csproj`), project),
            ]);
            // The game discovers the mod as soon as its manifest exists.
            const pendingManifest = path.join(modDirectory, 'mod.json.tmp');
            await fs.writeFile(pendingManifest, JSON.stringify(manifest, null, 2) + '\n');
            await fs.rename(pendingManifest, path.join(modDirectory, 'mod.json'));

            const open = await vscode.window.showInformationMessage(
                '모드를 만들었습니다. 게임의 시작 화면에서 편집 메뉴를 다시 열면 사용할 수 있습니다.',
                '모드 폴더 열기',
            );
            if (open)
                await vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(modDirectory), true);
            else
                await vscode.window.showTextDocument(vscode.Uri.file(sourcePath));
        } catch (error) {
            await vscode.window.showErrorMessage(`S8 모드 만들기 실패 — ${error.message}`);
        }
    }));
}

module.exports = { activate };
