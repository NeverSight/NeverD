package fixture;

import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;

public final class PackageClosedConstructorParentHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned package closed constructor parent mismatch");
    }
    private static final class Observer extends Trace {
        final Source source;
        final int mode, initialFirst, initialSecond;
        final Throwable failure;
        final List<Integer> events = new ArrayList<Integer>();
        final Carrier[] allocated = new Carrier[3];
        Core partial;
        Observer(Source source, int mode, boolean error) {
            this.source = source;
            this.mode = mode;
            initialFirst = source == null ? 0 : source.first;
            initialSecond = source == null ? 0 : source.second;
            failure = error ? new LinkageError("owned failure") : new IllegalArgumentException("owned failure");
        }
        @Override public void observe(int phase, Parent owner, Object value, int number) {
            require(phase == events.size());
            events.add(Integer.valueOf(phase));
            if (phase < 2) {
                require(owner == null && value == source && source != null);
                require(number == (phase == 0 ? initialFirst : initialSecond));
                if (phase == 0) source.first = 31;
                else {
                    require(source.first == 31);
                    source.first = -99;
                    source.second = 47;
                }
            } else if (phase < 5) {
                require(owner == null && value instanceof Carrier && number == phase);
                Carrier carrier = (Carrier)value;
                require(carrier.value == phase);
                for (Carrier previous : allocated) require(previous != carrier);
                allocated[phase - 2] = carrier;
            } else {
                require(owner instanceof Core);
                Core core = (Core)owner;
                if (phase == 5) partial = core;
                require(core == partial);
                require(core.first == (phase > 5 ? allocated[0] : null));
                require(core.second == (phase > 6 ? allocated[1] : null));
                require(core.third == (phase > 7 ? allocated[2] : null));
                if (phase < 8) require(value == allocated[phase - 5] && number == 0);
                else if (phase == 8) require(value == null && number == 0);
                if (phase < 9) require(core.source == null && core.left == 0 && core.right == 0 && core.label == null);
                else require(value == source && number == 78 && core.source == source
                             && core.left == 31 && core.right == 47 && "owned".equals(core.label));
            }
            if (mode == phase + 1) {
                if (failure instanceof Error) throw (Error)failure;
                throw (RuntimeException)failure;
            }
        }
    }
    private static Core call(int route, Source source) throws Throwable {
        if (route == 0) return new Core(source);
        try {
            java.lang.reflect.Constructor<Core> constructor = Core.class.getDeclaredConstructor(Source.class);
            constructor.setAccessible(true);
            return constructor.newInstance(source);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]);
        int shape = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        boolean error = Integer.parseInt(args[3]) != 0;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Source source = shape == 0 ? null : new Source();
            if (source != null) { source.first = 7 + attempt; source.second = 13 + attempt; }
            Observer observer = new Observer(source, mode, error);
            Trace.hook = observer;
            Core result = null;
            Throwable caught = null;
            try { result = call(route, source); } catch (Throwable failure) { caught = failure; }
            int last = source == null ? -1 : mode == 0 ? 9 : mode - 1;
            require(observer.events.size() == last + 1);
            for (int phase = 0; phase <= last; ++phase) require(observer.events.get(phase).intValue() == phase);
            if (source == null) require(caught != null && caught.getClass() == NullPointerException.class);
            else if (mode != 0) require(caught == observer.failure);
            else require(caught == null && result == observer.partial);
            if (caught != null) require(result == null);
            for (int index = 0; index < 3; ++index) {
                require((observer.allocated[index] != null) == (last >= index + 2));
                if (observer.allocated[index] != null) require(observer.allocated[index].value == index + 2);
            }
            require((observer.partial != null) == (last >= 5));
            if (observer.partial != null) {
                Core core = observer.partial;
                require(core.first == (last > 5 ? observer.allocated[0] : null));
                require(core.second == (last > 6 ? observer.allocated[1] : null));
                require(core.third == (last > 7 ? observer.allocated[2] : null));
                require(core.source == (last == 9 ? source : null));
                require(core.left == (last == 9 ? 31 : 0) && core.right == (last == 9 ? 47 : 0));
                require(last == 9 ? "owned".equals(core.label) : core.label == null);
            }
        }
        System.out.println("attempts=2,package-closed-constructor-parent=matched");
    }
}
