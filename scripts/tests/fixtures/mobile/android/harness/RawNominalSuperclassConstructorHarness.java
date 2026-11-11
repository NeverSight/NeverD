import fixture.Child;
import fixture.Config;
import fixture.Core;
import fixture.Trace;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;
import java.lang.reflect.Type;

public final class RawNominalSuperclassConstructorHarness {
    private static final class ProbeMap extends HashMap<Object,Object> {
        @Override public Object get(Object key) { throw new AssertionError("owned map observed"); }
        @Override public Object put(Object key, Object value) { throw new AssertionError("owned map observed"); }
        @Override public int size() { throw new AssertionError("owned map observed"); }
        @Override public Set<Map.Entry<Object,Object>> entrySet() { throw new AssertionError("owned map observed"); }
    }
    private static final class ProbeType implements Type {
        @Override public String getTypeName() { throw new AssertionError("owned type observed"); }
    }
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned raw nominal superclass constructor mismatch: " + detail);
    }
    private static void attempt(int route, final int mode, int attempt) {
        final Map target = mode == 2 ? null : new ProbeMap();
        final Object source = mode == 1 ? null : mode == 9 ? target : new Object();
        final Type type = mode == 10 ? null : mode == 11 ? new ProbeType() : attempt == 0 ? String.class : Object.class;
        final Object tag = new Object(), marker = new Object(), replacement = new Object();
        final RuntimeException failure = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
        final int[] callbacks = {0};
        final Config config = mode == 3 ? null : new Config(tag,null);
        Trace.source = Trace.tag = null;
        Trace.target = null;
        Trace.config = null;
        Trace.type = null;
        Trace.marker = marker;
        Trace.nextSource = source; Trace.nextTarget = target; Trace.nextType = type; Trace.nextConfig = config;
        Trace.bases = Trace.children = Trace.stage = 0;
        if (config != null && mode != 4) config.action = () -> {
            callbacks[0]++;
            check(Trace.bases == 1 && Trace.children == 0 && Trace.stage == 2, "parent before child stores");
            check(Trace.source == source && Trace.target == target && Trace.config == config && Trace.tag == tag, "original argument references");
            check(Trace.type == null, "type store after parent");
            if (mode == 5) throw failure;
            if (mode == 6) throw error;
            if (mode == 7) throw (RuntimeException)null;
            if (mode == 8) {
                Trace.nextSource = replacement; Trace.nextTarget = new ProbeMap(); Trace.nextType = Integer.class;
                Trace.nextConfig = new Config(replacement,null); Trace.marker = replacement; config.tag = replacement;
            }
        };
        Object value = null;
        Throwable thrown = null;
        try {
            switch (route) {
                case 0: value = Core.make(source,target,type,config); break;
                case 1: value = Core.target(source,target,type,config); break;
                case 2: value = Core.source(source,target,type,config); break;
                case 3: value = Core.type(source,target,type,config); break;
                case 4: value = Core.ignored(source,target,type,config); break;
                default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { thrown = caught; }
        boolean failed = mode >= 3 && mode <= 7;
        if (mode == 3 || mode == 4 || mode == 7) check(thrown instanceof NullPointerException, "null failure order");
        else if (mode == 5) check(thrown == failure, "runtime identity");
        else if (mode == 6) check(thrown == error, "Error identity");
        else {
            check(thrown == null, "unexpected failure");
            if (route == 0) {
                check(value instanceof Child, "constructed identity");
                Child child = (Child)value;
                check(child.source == source && child.target == target && child.config == config && child.type == type, "final stores");
            } else check(value == (route == 1 ? target : route == 2 ? source : route == 3 ? type : mode == 8 ? replacement : marker), "actual returned reference");
        }
        check(Trace.bases == 1 && Trace.children == (failed ? 0 : 1), "one exact parent and child");
        check(callbacks[0] == (mode == 3 || mode == 4 ? 0 : 1), "one callback after config reads");
        check(Trace.source == source && Trace.target == target && Trace.config == config, "argument snapshot");
        check(Trace.tag == (mode == 3 ? null : tag), "tag before callback mutation");
        check(Trace.type == (failed ? null : type), "original type after parent");
        check(Trace.stage == (mode == 3 ? 1 : failed ? 2 : 4), "constructor effect order");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route,mode,attempt);
        System.out.println("attempts=2,raw-nominal-superclass-constructor=matched");
    }
}
