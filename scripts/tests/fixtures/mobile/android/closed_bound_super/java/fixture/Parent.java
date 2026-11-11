package fixture;
public class Parent<M extends Service> {
    public final M manager;
    protected Parent(M manager) {
        Trace.parents++;Trace.hit(1);this.manager=manager;Trace.partial=this;
        Trace.action.run();Trace.hit(2);
    }
}
