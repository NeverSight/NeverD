package fixture;
public abstract class Owner<S extends Owner<S>> {
    public static Target value;
    static {
        Trace.hit(1);
        value = new Target(Trace.context(), new Callback() {
            public boolean call(Object value) {
                Trace.hit(5);
                Trace.receiver = this;
                Trace.argument = value;
                Trace.action.run();
                Trace.hit(6);
                return Trace.flag;
            }
        });
        Trace.hit(9);
    }
}
