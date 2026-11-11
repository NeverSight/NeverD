package fixture;
public final class Core {
    private Core() {}
    public static <E> Widget<E> make(java.util.Collection<E> source) {
        try {
            if(source == null)return new Widget();
            return new Widget(source);
        } catch(Exception failure) {
            Trace.caught=failure;Trace.hit(8);return null;
        }
    }
}
