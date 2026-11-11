import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class NullableArrayLoopHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("nullable array loop contract"); }
    static String[] copy(int route, String[] input) throws Throwable {
        try {
            if (route == 0) return Core.copy(input);
            if (route == 1) return NullableArrayLoopCaller.copy(input);
            if (route == 2) return (String[]) Core.class.getMethod("copy", String[].class).invoke(null, (Object) input);
            return (String[]) MethodHandles.publicLookup().findStatic(Core.class, "copy",
                MethodType.methodType(String[].class, String[].class)).invokeExact(input);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int[] lengths = {-1, 0, 1, 2, 5, 3, 4, 2};
        int length = lengths[variant], expectedVisits = 0;
        Provider.failure = new IllegalStateException("owned nullable loop failure");
        Provider.failAt = variant == 5 ? 1 : variant == 6 ? 2 : variant == 7 ? 3 : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            String[] input = length < 0 ? null : new String[length];
            if (input != null) Arrays.fill(input, "untouched");
            String[] expected = input == null ? null : input.clone();
            boolean shouldFail = false;
            for (int i = 0; i < Math.max(0, length); i++) {
                expectedVisits++;
                if (expectedVisits == Provider.failAt) { shouldFail = true; break; }
                expected[i] = i % 2 == 0 ? "alpha" : null;
            }
            Throwable caught = null;
            try { require(copy(route, input) == input && !shouldFail); }
            catch (Throwable failure) { caught = failure; }
            require(shouldFail ? caught == Provider.failure : caught == null);
            require(Provider.visits == expectedVisits && Arrays.equals(input, expected));
        }
        System.out.println("variant=" + variant + ",route=" + route + ",visits=" + Provider.visits);
    }
}
