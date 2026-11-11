package fixture;
public final class Owner {
    public Object replacement;
    public class Child extends Middle {
        public void run(Box<Object> box, Object token, int code) {
            Trace.hit(1);
            if (replacement != null) super.apply(box, replacement, code);
            else super.apply(box, token, code);
            Trace.hit(4);
        }
        public void apply(Box<Object> box, Object token, int code) { Trace.hit(8); }
    }
}
