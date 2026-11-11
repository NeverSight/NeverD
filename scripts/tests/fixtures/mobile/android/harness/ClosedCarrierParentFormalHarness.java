import fixture.Base;
import fixture.Core;
import fixture.Payload;
import fixture.Trace;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

public final class ClosedCarrierParentFormalHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed carrier parent formal mismatch");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void check(int route, final int mode) {
        final List<Payload> values = mode == 1 ? null : new ArrayList<Payload>();
        final Collection<Payload> extras = mode == 2 ? null : mode == 11 ? values : new ArrayList<Payload>();
        final Payload payload = new Payload();
        final Object poison = new Object();
        if (mode == 12) values.add(null);
        if (mode == 13) ((List)values).add(poison);
        if (mode == 14) ((Collection)extras).add(poison);
        final int originalSize = values == null ? 0 : values.size();
        final int extraSize = extras == null ? 0 : extras.size();
        final Object token = mode == 3 ? null : new Object();
        final Object root = new Object(), context = mode == 17 ? token : new Object();
        final List<Payload> changed = new ArrayList<Payload>();
        final RuntimeException runtime = new IllegalArgumentException("action");
        final Error error = new AssertionError("action");
        final int[] actions = {0};
        final Runnable action = new Runnable() {
            public void run() {
                actions[0]++;
                require(Trace.targets == 1 && Trace.children == 0 && Trace.stage == 2);
                require(Trace.before == 0 && Trace.active != null && Trace.active.get() == null);
                require(Trace.active.value == null && Trace.active.extras == null && Trace.active.token == null);
                Core active = (Core)Trace.active;
                require(active.state() == 0 && active.code == 0 && active.root == null && active.context == null);
                require(Trace.source == values && Trace.extras == extras && Trace.token == token);
                if (mode == 5) throw runtime;
                if (mode == 6) throw error;
                if (mode == 7) throw null;
                if (mode == 8) { Trace.source = changed; Trace.extras = changed; Trace.token = poison; }
                if (mode == 9) values.add(payload);
                if (mode == 10) extras.add(payload);
                if (mode == 15) Trace.action = null;
                if (mode == 18) { values.add(payload); throw runtime; }
                if (mode == 19) { extras.add(payload); throw error; }
            }
        };
        Trace.targets = Trace.children = Trace.stage = Trace.before = 0;
        Trace.active = null; Trace.source = Trace.token = null; Trace.extras = null;
        Trace.action = mode == 4 ? null : action;
        final int left = mode == 16 ? Integer.MAX_VALUE : mode == 17 ? Integer.MIN_VALUE : 7;
        final int right = route == 0 ? 3 : -9;
        Core result = null; Throwable failure = null;
        try {
            result = route == 0 ? new Core(values, Trace.action, extras, token, left)
                : Core.make(root, left, right, values, Trace.action, context, extras, token);
        } catch (Throwable caught) { failure = caught; }
        final boolean failed = mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 18 || mode == 19;
        require(Trace.targets == 1 && Trace.children == (failed ? 0 : 1));
        require(Trace.stage == (failed ? 2 : 4) && Trace.before == 0);
        require(actions[0] == (mode == 4 ? 0 : 1));
        require(Trace.source == (mode == 8 ? changed : values));
        require(Trace.extras == (mode == 8 ? changed : extras));
        require(Trace.token == (mode == 8 ? poison : token));
        require(Trace.action == (mode == 4 || mode == 15 ? null : action));
        require(Trace.active != null);
        Core active = (Core)Trace.active;
        if (mode == 4 || mode == 7) require(failure instanceof NullPointerException);
        else if (mode == 5 || mode == 18) require(failure == runtime);
        else if (mode == 6 || mode == 19) require(failure == error);
        else require(failure == null && result == active);
        if (failed) {
            require(result == null && active.value == null && active.get() == null);
            require(active.extras == null && active.token == null && active.state() == 0);
            require(active.root == null && active.context == null);
        } else {
            require(active.get() == values && active.value == values && active.extras == extras && active.token == token);
            require(active.state() == left * 31 + right);
            require(active.root == (route == 0 ? token : root));
            require(active.context == (route == 0 ? token : context));
        }
        if (values != null) require(values.size() == originalSize + (mode == 9 || mode == 18 ? 1 : 0));
        if (extras != null) require(extras.size() == extraSize + (mode == 10 || mode == 19 ? 1 : 0));
        if (mode == 13) require(((List)values).get(0) == poison);
        if (mode == 14) require(((Collection)extras).iterator().next() == poison);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(route, mode); check(route, mode);
        System.out.println("attempts=2,closed-carrier-parent-formal=matched");
    }
}
