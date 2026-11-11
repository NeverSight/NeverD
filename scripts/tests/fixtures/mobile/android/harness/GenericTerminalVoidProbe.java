public class GenericTerminalVoidProbe {
    public static int touches;
    public int hashCode(){touches++;throw new IllegalArgumentException("owned operand");}
    public String toString(){touches++;throw new IllegalArgumentException("owned operand");}
    public static final class Child extends GenericTerminalVoidProbe {}
}
