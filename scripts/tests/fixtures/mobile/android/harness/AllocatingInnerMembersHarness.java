import fixture.Carrier;
import fixture.InnerHost;
import fixture.OuterProbe;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public class AllocatingInnerMembersHarness {
    public static void main(String[] args) throws Exception {
        InnerHost<String> host = new InnerHost<String>(9);
        OuterProbe.fail = true;
        try { host.make(1); throw new AssertionError("allocation constructor did not fail"); }
        catch (IllegalStateException expected) {
            if (OuterProbe.trace != 123 || OuterProbe.allocations != 1 || OuterProbe.constructions != 0)
                throw new AssertionError("class initialization or early constructor failure was reordered");
            System.out.println("failure:" + OuterProbe.trace);
        }
        OuterProbe.fail = false;
        for (int value : new int[] {0, 7, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            OuterProbe.trace = 0;
            int allocations = OuterProbe.allocations;
            int parents = OuterProbe.constructions;
            InnerHost.Member member = host.make(value);
            Carrier carrier = new Carrier();
            carrier.member = member;
            if (OuterProbe.trace != 134 || OuterProbe.allocations != allocations + 1
                    || OuterProbe.constructions != parents + 1 || member.token == null
                    || member.retained != member.token || member.observed != host.base
                    || member.offset != value || carrier.member != member)
                throw new AssertionError("allocation identity, count or parent arguments changed");
            Field capture = member.getClass().getDeclaredField("this$0");
            capture.setAccessible(true);
            if (capture.get(member) != host) throw new AssertionError("outer capture lost identity");
            host.base++;
            if (member.readOuter() != host.base) throw new AssertionError("capture stopped being live");
            System.out.println(value + ":" + OuterProbe.trace + ":" + OuterProbe.allocations);
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
