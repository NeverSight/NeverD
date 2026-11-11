package fixture;
public final class Caller {
    public static Core<String> copy(Core<String> self) throws CloneNotSupportedException {
        return self.clone();
    }
}
