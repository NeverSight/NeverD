package fixture;
public class Child extends Base<Child, Child.Left, Child.Right> {
    public interface Left extends First<Child> { }
    public interface Right extends Second<Child> { }
}
