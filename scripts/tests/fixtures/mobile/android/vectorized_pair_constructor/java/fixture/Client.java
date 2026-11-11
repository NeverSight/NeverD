package fixture;
public final class Client {
    @SuppressWarnings("unchecked")
    public static Core create(Spec spec,Link converter,Object first,Object second,Bound velocity) { return new Core(spec,converter,first,second,velocity); }
}
