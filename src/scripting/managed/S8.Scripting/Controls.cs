namespace S8.Scripting;

// These components compose the shared native dialog service. They are managed
// behaviours, not one-to-one declarations of C++ engine classes.
public sealed class ButtonComponent : Component
{
    private DialogComponent? owner;
    private int action;
    public Action? Clicked;

    public void Bind(DialogComponent dialog, int actionId)
    {
        if (owner != null)
            owner.Clicked -= OnClick;

        owner = dialog;
        action = actionId;
        owner.Clicked += OnClick;
    }

    private void OnClick(int value)
    {
        if (value == action)
            Clicked?.Invoke();
    }

    public override void OnDisable()
    {
        if (owner != null)
            owner.Clicked -= OnClick;

        owner = null;
        Clicked = null;
    }
}

public sealed class TextLabelComponent : Component
{
    private DialogComponent owner = null!;
    private int action;
    private string label = "", text = "";

    public void Bind(DialogComponent dialog, int actionId, string caption)
    {
        owner = dialog;
        action = actionId;
        label = caption;
    }

    public string Text
    {
        get => text;
        set
        {
            owner.SetRow(new(action, label, value));
            text = value;
        }
    }
}

public sealed class TextInputComponent : Component
{
    private DialogComponent? owner;
    public Action<InputResult, string>? Completed;

    public void Bind(DialogComponent dialog)
    {
        if (owner != null)
            owner.TextCompleted -= OnCompleted;

        owner = dialog;
        owner.TextCompleted += OnCompleted;
    }

    private void OnCompleted(InputResult result, string value) => Completed?.Invoke(result, value);

    public void Show(string initial, int maximumCharacters, bool multiline = false) => owner!.EditText(initial, maximumCharacters, multiline);

    public override void OnDisable()
    {
        if (owner != null)
            owner.TextCompleted -= OnCompleted;

        owner = null;
        Completed = null;
    }
}

public sealed class NumberInputComponent : Component
{
    private DialogComponent? owner;
    public Action<bool, int>? Completed;

    public void Bind(DialogComponent dialog)
    {
        if (owner != null)
            owner.NumberCompleted -= OnCompleted;

        owner = dialog;
        owner.NumberCompleted += OnCompleted;
    }

    private void OnCompleted(bool accepted, int value) => Completed?.Invoke(accepted, value);

    public void Show(int initial, int minimum = 0, int maximum = 65535) => owner!.EditNumber(initial, minimum, maximum);

    public override void OnDisable()
    {
        if (owner != null)
            owner.NumberCompleted -= OnCompleted;

        owner = null;
        Completed = null;
    }
}
