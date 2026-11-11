import fixture.SharedHost;
import consumer.Remote;
import java.lang.reflect.Modifier;

public class SharedAnonymousHarness {
    public static void main(String[] args) {
        int created = 0;
        for (int input : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Object first = SharedHost.make(input);
            Object second = SharedHost.another(input + 1);
            Object third = Remote.make(input + 2);
            created += 3;
            if (first == second || first.getClass() != second.getClass()
                    || first.getClass() != third.getClass()
                    || SharedHost.read(first) != input
                    || SharedHost.read(second) != input + 1
                    || Remote.read(third) != input + 2
                    || SharedHost.wrap(second).value() != input + 1
                    || SharedHost.count() != created) {
                throw new AssertionError("Shared anonymous class identity or state changed");
            }
            Class<?> type = first.getClass();
            if (type.getEnclosingClass() != SharedHost.class
                    || type.getEnclosingMethod() != null
                    || !type.isMemberClass() || type.isAnonymousClass()
                    || !Modifier.isPublic(type.getModifiers())
                    || !Modifier.isStatic(type.getModifiers())) {
                throw new AssertionError("Reported shared member projection is not present");
            }
            System.out.println(input + ":" + SharedHost.read(second) + ":" + SharedHost.count());
        }
    }
}
