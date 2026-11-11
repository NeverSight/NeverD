package fixture;
public final class Owner {
    static {Trace.hit(1);}
    private static <T> Box<T> make() {
        Trace.hit(2);
        Box<T> allocated=new Box<T>(Trace.values,(T)Trace.seed);
        Trace.allocated=allocated;Trace.hit(6);
        return Trace.override?(Box<T>)Trace.result:allocated;
    }
    public static Box<?> call() {return make();}
    public static String text() {return (String)make().get();}
}
