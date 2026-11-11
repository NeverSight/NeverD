import fixture.Duo;
import fixture.LambdaScope;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;

public final class LambdaScopeHarness {
    private static int firstCalls, secondCalls;
    private static final RuntimeException failure = new RuntimeException("owned sentinel");

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Lambda scope behavior changed");
    }

    public static void main(String[] args) throws Exception {
        boolean original = args[0].equals("original");
        Object[] values = {new Object(), "value", Integer.valueOf(53), null};
        for (int i = 0; i < values.length; ++i) {
            final Object left = values[i], right = values[(i + 1) % values.length];
            Duo<Object, Object> source = new Duo<Object, Object>() {
                public Object first() { ++firstCalls; return left; }
                public Object second() { ++secondCalls; return right; }
                public Object replace(Object v) { throw new AssertionError("unexpected mutation"); }
            };
            Duo<Object, Object> result = LambdaScope.flip(source);
            require(result != source && result.first() == right && result.second() == left);
            require(result.replace(left) == left && result.replace(right) == right);
            require(firstCalls == i + 1 && secondCalls == i + 1);
            require(result.getClass().getEnclosingMethod().getName().equals("lambda$flip$0"));
            try {
                Type argument = ((ParameterizedType) result.getClass().getGenericInterfaces()[0]).getActualTypeArguments()[0];
                require(original ? argument == null : argument == Object.class);
            } catch (TypeNotPresentException unavailable) {
                require(original && unavailable.typeName().equals("T"));
            }
        }
        Duo<Object, Object> throwing = new Duo<Object, Object>() {
            public Object first() { throw failure; }
            public Object second() { throw failure; }
            public Object replace(Object v) { throw new AssertionError(); }
        };
        Duo<Object, Object> failed = LambdaScope.flip(throwing);
        for (int i = 0; i < 2; ++i) {
            try {
                if (i == 0) failed.first(); else failed.second();
                throw new AssertionError("missing original callback failure");
            } catch (RuntimeException actual) { require(actual == failure); }
        }
        Duo<Object, Object> absent = LambdaScope.flip(null);
        require(absent.replace(values[0]) == values[0]);
        for (int i = 0; i < 2; ++i) {
            try {
                if (i == 0) absent.first(); else absent.second();
                throw new AssertionError("missing null capture failure");
            } catch (NullPointerException expected) {}
        }
        System.out.println("calls=8;forwarded=16;callback-failures=2;null-failures=2;captured-identity=matched");
    }
}
