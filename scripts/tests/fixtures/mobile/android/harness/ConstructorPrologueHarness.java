import fixture.Captured;
import java.lang.reflect.Modifier;

public class ConstructorPrologueHarness {
    public static void main(String[] args) throws Throwable {
        Object token = new Object();
        for (int value : new int[] {Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Captured object = new Captured(value, token);
            if (object.seen != value + 3 || object.extra != value + 3
                    || object.captured != value || object.token != token) {
                throw new AssertionError("Constructor evaluation order changed");
            }
            System.out.println(value + ":" + object.seen + ":" + object.extra);
        }
        for (String field : new String[] {"captured", "token", "extra", "seen"}) {
            if (!Modifier.isFinal(Captured.class.getField(field).getModifiers())) {
                throw new AssertionError("Final field modifier was dropped");
            }
        }
        System.out.println("final-fields-and-reference-identity:preserved");
    }
}
