package fixture;
public class Owner extends Middle {
    public <U extends First & Second> void accept(U value) {
        super.accept(value);
        Trace.hit(6);
        Trace.after.run();
        Trace.hit(7);
    }
}
