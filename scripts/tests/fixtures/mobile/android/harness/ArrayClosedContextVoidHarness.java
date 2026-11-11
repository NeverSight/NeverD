import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class ArrayClosedContextVoidHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned array closed context void");
    }
    static String call(int route, Object[] values, StringBuilder text, Bucket<Object[]> path) throws Throwable {
        if (route == 0) return Owner.render(values, text, path);
        if (route == 1) return ArrayClosedContextVoidCaller.render(values, text, path);
        if (route == 2) {
            try { return (String) Owner.class.getMethod("render", Object[].class, StringBuilder.class, Bucket.class).invoke(null, values, text, path); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (String) MethodHandles.publicLookup().findStatic(Owner.class, "render",
            MethodType.methodType(String.class, Object[].class, StringBuilder.class, Bucket.class)).invokeExact(values, text, path);
    }
    static final class Element {
        final int mode;
        Object[] root;
        int calls;
        Element(int mode) { this.mode = mode; }
        public String toString() {
            calls++;
            if (mode == 8 || mode == 21) throw Trace.failure;
            if (mode == 9) throw Trace.error;
            if (mode == 19) root[1] = "late";
            return "token";
        }
    }
    static final class PathBucket implements Bucket<Object[]> {
        final int mode;
        final Object[] root, child;
        final List<Object[]> stack = new ArrayList<Object[]>();
        int contains, pushes, pops;
        PathBucket(int mode, Object[] root, Object[] child) { this.mode = mode; this.root = root; this.child = child; }
        public boolean contains(Object[] value) {
            contains++;
            require(value == root || child != null && value == child);
            if (mode == 11) throw Trace.failure;
            if (mode == 22) value[0] = "changed";
            for (Object[] item : stack) if (item == value) return true;
            return false;
        }
        public void push(Object[] value) {
            pushes++;
            require(value == root || child != null && value == child);
            if (mode == 10) value[0] = new Object();
            if (mode == 12) throw Trace.error;
            stack.add(value);
        }
        public Object[] pop() {
            pops++;
            if (mode == 13) throw Trace.failure;
            Object[] result = stack.remove(stack.size() - 1);
            return mode == 23 ? new Object[]{new Object()} : result;
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.entries = 0; Trace.exits = 0;
            Trace.failAt = mode == 14 ? 3 : mode == 15 ? 1 : mode == 16 ? 2 : mode == 17 ? 4 : 0;
            Trace.failure = new RuntimeException("owned traversal failure");
            Trace.error = new AssertionError("owned traversal error");
            Element element = new Element(mode);
            Object[] child = new Object[]{"nested"};
            Object[] values = mode == 1 || mode == 20 ? new String[0] : mode == 2 || mode == 10 ? new String[]{"a", "b"}
                : mode == 3 ? new Object[]{child, child} : mode == 4 ? new Object[1] : mode == 5 ? null
                : mode == 21 ? new Object[]{child, element} : new Object[]{element, null};
            element.root = values;
            if (mode == 4) values[0] = values;
            StringBuilder text = mode == 6 ? null : new StringBuilder();
            PathBucket bucket = new PathBucket(mode, values, child);
            if (mode == 18) bucket.stack.add(values);
            String answer = null; Throwable failure = null;
            try { answer = call(route, values, text, mode == 7 ? null : bucket); } catch (Throwable caught) { failure = caught; }
            long events = mode == 14 ? 3 : mode == 15 || mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 9
                || mode == 10 || mode == 11 || mode == 12 || mode == 13 ? 31 : mode == 16 ? 312
                : mode == 21 ? 3112 : mode == 3 ? 31121224 : mode == 4 ? 31124 : mode == 18 ? 314 : 3124;
            require(Trace.events == events);
            int entries = mode == 14 ? 0 : mode == 3 ? 3 : mode == 4 || mode == 21 ? 2 : 1;
            int exits = mode == 3 ? 3 : mode == 4 || mode == 21 ? 1 : mode == 16 || mode == 17
                || mode < 3 || mode == 19 || mode == 20 || mode == 22 || mode == 23 ? 1 : 0;
            require(Trace.entries == entries && Trace.exits == exits);
            int contains = mode == 14 || mode == 15 || mode == 7 ? 0 : entries;
            int pushes = mode == 11 || mode == 18 ? 0 : mode == 4 ? 1 : contains;
            int pops = mode == 14 || mode == 15 || mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 9
                || mode == 10 || mode == 11 || mode == 12 || mode == 18 ? 0 : mode == 3 ? 3 : 1;
            require(bucket.contains == contains && bucket.pushes == pushes && bucket.pops == pops);
            int remaining = mode == 5 || mode == 6 || mode == 8 || mode == 9 || mode == 13 || mode == 18 || mode == 21 ? 1 : 0;
            require(bucket.stack.size() == remaining && (remaining == 0 || bucket.stack.get(0) == values));
            String expected = mode == 1 || mode == 20 ? "[]" : mode == 2 ? "[a,b]" : mode == 3 ? "[[nested],[nested]]"
                : mode == 4 ? "[!]" : mode == 5 || mode == 8 || mode == 9 ? "[" : mode == 6 ? null
                : mode == 7 || mode == 10 || mode == 11 || mode == 12 || mode == 14 || mode == 15 ? ""
                : mode == 18 ? "!" : mode == 19 ? "[token,late]" : mode == 21 ? "[[nested],"
                : mode == 22 ? "[changed,null]" : "[token,null]";
            require(text == null ? expected == null : text.toString().equals(expected));
            if (mode == 5 || mode == 6 || mode == 7) require(failure instanceof NullPointerException && answer == null);
            else if (mode == 10) require(failure instanceof ArrayStoreException && "a".equals(values[0]) && answer == null);
            else if (mode == 9 || mode == 12) require(failure == Trace.error && answer == null);
            else if (mode == 8 || mode == 11 || mode == 13 || mode == 14 || mode == 15 || mode == 16 || mode == 17 || mode == 21)
                require(failure == Trace.failure && answer == null);
            else require(failure == null && answer.equals(expected));
            int strings = mode == 0 || mode == 8 || mode == 9 || mode == 13 || mode == 16 || mode == 17 || mode == 19 || mode == 21 || mode == 23 ? 1 : 0;
            require(element.calls == strings);
        }
        System.out.println("attempts=2,array-closed-context-void=matched");
    }
}
