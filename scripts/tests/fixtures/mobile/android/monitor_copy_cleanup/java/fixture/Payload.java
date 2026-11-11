package fixture;
public final class Payload {
    public Object anchor;
    public static Payload partial;
    public static Runnable observer;
    public Payload(Object value) {
        anchor=value;partial=this;
        if(observer!=null)observer.run();
    }
}
