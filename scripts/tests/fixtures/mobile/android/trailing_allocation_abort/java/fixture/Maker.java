package fixture;
public final class Maker {
    public static Outer make(float first, float second, int mode) {
        return new Outer(new Inner(switch (mode) {
            case 0 -> {
                if (Provider.current instanceof Checked) yield ((Checked)Provider.current).read();
                throw new Failure("owned input");
            }
            default -> throw new Failure("owned mode");
        }, first, second, Provider.style()));
    }
}
