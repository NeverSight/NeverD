package fixture;
public final class Core {
    public static Node call(Object value,Context previous){return Node.make(value,previous);}
    public static Node checked(Object value,Object previous){return call(value,(Context)previous);}
}
