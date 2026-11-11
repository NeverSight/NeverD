import fixture.InnerHost;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public class InnerMembersHarness {
    public static void main(String[] args) throws Exception {
        for (int base : new int[] {0, 7, -3, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            InnerHost host = new InnerHost(base);
            for (int offset : new int[] {4, -9}) {
                host.base = base;
                InnerHost.Member member = host.make(offset);
                if (member.observed != base || member.result() != base + offset)
                    throw new AssertionError("outer capture was not ready before super");
                Field capture = member.getClass().getDeclaredField("this$0");
                capture.setAccessible(true);
                if (capture.get(member) != host)
                    throw new AssertionError("outer instance identity changed");
                host.base = base ^ 37;
                if (member.readOuter() != host.base || member.observed != base)
                    throw new AssertionError("capture no longer shares outer state");
                member.getClass().getDeclaredConstructor(InnerHost.class, int.class);
                System.out.println(base + ":" + offset + ":" + member.result());
            }
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
