import fixture.InnerHost;
import fixture.OuterProbe;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public class DelegatingInnerMembersHarness {
    public static void main(String[] args) throws Exception {
        Constructor<InnerHost.Member> forwarding = InnerHost.Member.class.getDeclaredConstructor(
                InnerHost.class, int.class, long.class, Object.class);
        Field capture = InnerHost.Member.class.getDeclaredField("this$0");
        capture.setAccessible(true);
        for (int base : new int[] {0, 7, -3, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            InnerHost host = new InnerHost(base);
            for (long marker : new long[] {Long.MIN_VALUE, -7L, 0L, 0x123456789abcdefL, Long.MAX_VALUE}) {
                int offset = base ^ 137;
                OuterProbe.constructions = 0;
                InnerHost.Member[] members = {
                    host.make(offset, marker),
                    forwarding.newInstance(host, offset, marker, new Object())
                };
                if (OuterProbe.constructions != 2)
                    throw new AssertionError("delegation repeated or skipped superclass initialization");
                for (InnerHost.Member member : members) {
                    if (capture.get(member) != host || member.observed != base
                            || member.offset != offset || member.result() != base + offset
                            || member.marker != marker)
                        throw new AssertionError("delegation changed capture, final fields, or wide state");
                    host.base = base ^ 37;
                    if (member.readOuter() != host.base || member.observed != base)
                        throw new AssertionError("delegated capture lost shared outer state");
                    host.base = base;
                    System.out.println(base + ":" + offset + ":" + member.marker + ":" + member.result());
                }
            }
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
