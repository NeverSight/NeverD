package fixture;

@dependency.Note("owned-dependency")
public class ClasspathApp extends dependency.Base implements dependency.Contract {
    public ClasspathApp() { super(); }
    public int value(int input) { return super.plus(input); }
    public static dependency.Base.Member member(dependency.Base.Member input) {
        return input;
    }
}
