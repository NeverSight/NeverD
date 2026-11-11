package fixture;
public class Base {
    public void apply(Box<Object> box, Object token, int code) {
        Trace.hit(2);
        Trace.receiver = this;
        Trace.box = box;
        Trace.token = token;
        Trace.code = code;
        box.value = token;
        Trace.hit(3);
    }
    public void apply(Object box, Object token, int code) { Trace.hit(9); }
}
