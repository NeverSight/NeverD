package fixture;

@android.annotation.SuppressLint({"ClassIssue", "", "ClassIssue"})
public class LintFixture {
    @android.annotation.SuppressLint({})
    public int value;

    @android.annotation.SuppressLint({"ConstructorIssue"})
    public LintFixture(int initial) {
        value = initial;
    }

    @android.annotation.SuppressLint({"PrivateApi", "line\n\"\\", "", "\000", "\ud800"})
    public int plus(int delta) {
        return value + delta;
    }

    @android.annotation.SuppressLint({})
    public static int twice(int input) {
        return input * 2;
    }
}
