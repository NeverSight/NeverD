import fixture.InnerHost;
import fixture.Guard;
import java.lang.reflect.Modifier;

public class GuardedInnerMembersHarness {
    public static void main(String[] args) {
        InnerHost host = new InnerHost(23);
        for (int value : new int[] {-7, 0, 9}) {
            Guard.trace = 0;
            try {
                InnerHost.Member member = host.make(value);
                if (value <= 0 || Guard.trace != 123 || member.observed != 23
                        || member.result() != 23 + value)
                    throw new AssertionError("guard/capture/super order changed");
                System.out.println("success:" + member.result() + ":" + Guard.trace);
            } catch (IllegalArgumentException failure) {
                int expected = value < 0 ? 1 : 12;
                String message = value < 0 ? "first" : "second";
                if (Guard.trace != expected || !message.equals(failure.getMessage()))
                    throw new AssertionError("a failed check ran later effects");
                System.out.println("failure:" + message + ":" + Guard.trace);
            }
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
