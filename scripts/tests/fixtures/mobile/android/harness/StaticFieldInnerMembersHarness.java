import fixture.InnerHost;
import fixture.StaticValues;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public class StaticFieldInnerMembersHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Static constructor read behavior changed");
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 0 && args[0].equals("failure")) {
            InnerHost.fail = true;
            InnerHost.failure = new RuntimeException("owned initializer failure");
            InnerHost host = new InnerHost(7);
            try { host.make(19); throw new AssertionError("missing initialization failure"); }
            catch (ExceptionInInitializerError error) {
                require(error.getException() == InnerHost.failure && InnerHost.trace == 1);
            }
            try { host.make(19); throw new AssertionError("failed initialization retried"); }
            catch (NoClassDefFoundError expected) { require(InnerHost.trace == 1); }
            System.out.println("initialization-failure:identity-preserved;super-effects=0;attempts=1");
            return;
        }
        int index = 0;
        for (int base : new int[] {0, 7, -3, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            InnerHost host = new InnerHost(base);
            int initial = index == 0 ? 11 : base ^ 37;
            long wide = 0x12345678abcdef01L + index * 17;
            InnerHost token = index % 2 == 0 ? host : null;
            if (index != 0) {
                StaticValues.initial = initial;
                StaticValues.wide = wide;
                StaticValues.token = token;
            }
            InnerHost.trace = 0;
            InnerHost.Member member = host.make(19);
            require(member.observed == base && member.passed == initial && member.offset == initial + 19);
            require(member.passedWide == wide && member.retainedWide == wide);
            require(member.retainedToken == member.passedToken);
            if (index == 0) require(member.retainedToken != null && member.retainedToken.base == 91);
            else require(member.retainedToken == token);
            require(StaticValues.initial == initial + 7 && StaticValues.wide == wide + 13
                    && StaticValues.token == null);
            require(InnerHost.trace == (index == 0 ? 12 : 2));
            Field capture = member.getClass().getDeclaredField("this$0");
            capture.setAccessible(true);
            require(capture.get(member) == host);
            host.base = base ^ 71;
            require(member.readOuter() == host.base && member.passed == initial);
            System.out.println(base + ":" + member.result() + ":" + InnerHost.trace);
            ++index;
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
