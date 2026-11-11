package fixture;
import java.util.function.Supplier;
public class Core {
    public static Child make(Context context, CharSequence text) {
        return new Child(context,text);
    }
    public static class Child extends Base<String> {
        static { Trace.hit(2); }
        public CharSequence marker;
        public Child(Context context, CharSequence text) {
            super(new Callback(context,text));
            marker=text;
            Trace.child=this;
            Trace.hit(7);
        }
    }
    private static class Callback implements Supplier<String> {
        static { Trace.hit(3); }
        public Context context;
        public CharSequence text;
        public String snapshot;
        Callback(Context context, CharSequence text) {
            this.context=context;
            this.text=text;
            snapshot=context.reply;
            Trace.adapter=this;
            Trace.hit(4);
        }
        public String get() {
            Trace.hit(6);
            return snapshot;
        }
    }
}
