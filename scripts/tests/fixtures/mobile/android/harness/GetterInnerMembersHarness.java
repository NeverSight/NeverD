import fixture.InnerHost;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;

public class GetterInnerMembersHarness {
    public static void main(String[] args) throws Exception {
        for (int base : new int[] {0, 7, -3, Integer.MIN_VALUE, Integer.MAX_VALUE, 42}) {
            InnerHost host = new InnerHost(base);
            InnerHost.trace = 0;
            try {
                InnerHost.Member member = host.make(19);
                if (base == 42) throw new AssertionError("getter did not throw");
                if (member.observed != base || member.passed != base
                        || member.offset != base + 19 || InnerHost.trace != 1234
                        || member.passedWide != 0x12345678abcdef01L
                        || member.retainedWide != member.passedWide
                        || member.passedToken != host || member.retainedToken != host)
                    throw new AssertionError("getter order or retained result changed");
                Field capture = member.getClass().getDeclaredField("this$0");
                capture.setAccessible(true);
                if (capture.get(member) != host)
                    throw new AssertionError("outer identity changed");
                host.base = base ^ 37;
                if (member.readOuter() != host.base || member.passed != base)
                    throw new AssertionError("getter result was read again");
                System.out.println(base + ":" + member.result() + ":" + InnerHost.trace);
            } catch (IllegalStateException expected) {
                if (base != 42 || InnerHost.trace != 1)
                    throw new AssertionError("super ran after getter failure");
                System.out.println("throw:" + InnerHost.trace);
            }
        }
        InnerHost.trace = 0;
        try {
            InnerHost.Member.class.getDeclaredConstructor(InnerHost.class, int.class)
                    .newInstance(null, 2);
            throw new AssertionError("null outer was accepted");
        } catch (InvocationTargetException expected) {
            if (!(expected.getCause() instanceof NullPointerException) || InnerHost.trace != 0)
                throw new AssertionError("null getter receiver effects changed");
            System.out.println("null:" + InnerHost.trace);
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
