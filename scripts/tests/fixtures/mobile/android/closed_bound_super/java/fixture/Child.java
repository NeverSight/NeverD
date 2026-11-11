package fixture;
public class Child extends Parent<Service> {
    public final ClassLoader loader;
    public Child(ClassLoader loader,Service manager) {super(manager);this.loader=loader;Trace.hit(4);}
    public Object capture() {return manager.snapshot();}
}
