package fixture;
public final class Core {
    private Core() {}
    public static Widget make(int source) {
        try {
            if(source <= 0)return new Widget();
            return new Widget(source);
        } catch(Exception failure) {
            Trace.caught=failure;Trace.hit(8);return null;
        }
    }
}
