package fixture;

import java.io.IOException;
import java.util.List;
import java.util.function.Supplier;

public final class Core {
    private Core() {}
    public static Object latest;
    private static final class Value {
        final Object payload;
        private Value(Object payload) {this.payload=payload;}
    }
    private static final class Context implements Supplier<Object> {
        final Object payload;
        private Context(Object payload) {this.payload=payload;}
        public Object get() {Trace.hit(5);return payload;}
    }
    private static final class Entry implements Runnable {
        final Context context;
        final List<Value> values;
        Object marker;
        int stage;
        private Entry(Context context, List<Value> values, Probe probe) throws IOException {
            latest=this;
            this.context=context;
            this.values=values;
            stage=1;
            Trace.hit(1);
            marker=probe.observe(this,values,context);
            stage=2;
            if (Trace.checked!=null) throw Trace.checked;
            Trace.hit(2);
            stage=3;
        }
        public void run() {Trace.hit(4);}
    }
    public static Object value(Object payload) {return new Value(payload);}
    public static Object context(Object payload) {return new Context(payload);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Object create(Object context, List<?> values, Probe probe) throws IOException {
        return new Entry((Context)context,(List)values,probe);
    }
}
