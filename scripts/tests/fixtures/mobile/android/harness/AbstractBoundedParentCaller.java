import fixture.*;
public final class AbstractBoundedParentCaller {
    private static int values, identities, states, failures;
    private static final class Receiver extends Parent<String, Box<String>> {
        public void direct(String m, int i, Box<String> b, long w, boolean e) { super.read(m, i, b, w, e); }
    }
    private interface Call { void run(Receiver r, String m, int i, Box<String> b, long w, boolean e); }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("concrete parent identity changed");
        identities++;
    }
    private static void state(Receiver r, int before, int visits, int route, String m, int i, Box<String> b, long w, boolean e) {
        if (r.parentCalls != before + 1 || r.parentIndex != i || r.parentWide != w || r.parentEnabled != e
                || r.visits != visits + (route == 0 ? 1 : 0))
            throw new AssertionError("concrete parent body effects changed");
        same(r.parentMarker, m); same(r.parentSeen, b); states++;
    }
    public static void main(String[] args) {
        Call[] routes = {(r,m,i,b,w,e) -> r.dispatch(m,i,b,w,e), (r,m,i,b,w,e) -> r.direct(m,i,b,w,e)};
        Receiver r = new Receiver();
        Box<String>[] boxes = new Box[]{null, new Box<String>()};
        for (String m : new String[]{null,"owned"}) for (int i : new int[]{Integer.MIN_VALUE,0,Integer.MAX_VALUE})
            for (long w : new long[]{Long.MIN_VALUE,0,Long.MAX_VALUE}) for (boolean e : new boolean[]{false,true})
                for (Box<String> b : boxes) for (int route = 0; route < routes.length; route++) {
                    int before = r.parentCalls, visits = r.visits;
                    routes[route].run(r,m,i,b,w,e); state(r,before,visits,route,m,i,b,w,e); values++;
                }
        RuntimeException problem = new IllegalStateException("owned concrete parent failure");
        r.parentFailure = problem;
        for (Box<String> b : boxes) for (int route = 0; route < routes.length; route++) {
            int before = r.parentCalls, visits = r.visits;
            try { routes[route].run(r,"failure",-1,b,Long.MIN_VALUE,true); throw new AssertionError("failure missing"); }
            catch (RuntimeException actual) { same(actual,problem); failures++; }
            state(r,before,visits,route,"failure",-1,b,Long.MIN_VALUE,true);
        }
        for (Call route : routes) {
            try { route.run(null,null,0,null,0,false); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
    }
}
