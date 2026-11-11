import fixture.InnerHost;
import fixture.OuterProbe;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public class CastInnerMembersHarness {
    public static void main(String[] args) throws Exception {
        Object[] tokens = {null, "", new String("token"), new Object(), Integer.valueOf(7), new StringBuilder("text")};
        for (int i = 0; i < tokens.length; i++) {
            Object token = tokens[i];
            InnerHost host = new InnerHost();
            host.base = 17 + i;
            InnerHost.trace = 0;
            OuterProbe.constructions = 0;
            try {
                InnerHost.Member member = host.make(token);
                if (i >= 3 || InnerHost.trace != 123 || OuterProbe.constructions != 1
                        || member.observed != host.base || member.passed != token
                        || member.retained != token || member.owner != host)
                    throw new AssertionError("cast, identity or constructor order changed");
                Field capture = member.getClass().getDeclaredField("this$0");
                capture.setAccessible(true);
                if (capture.get(member) != host)
                    throw new AssertionError("outer capture lost identity");
                host.base++;
                if (member.readOuter() != host.base || member.observed != 17 + i)
                    throw new AssertionError("outer capture stopped being live");
                System.out.println(i + ":ok:" + InnerHost.trace);
            } catch (ClassCastException expected) {
                if (i < 3 || InnerHost.trace != 1 || OuterProbe.constructions != 0)
                    throw new AssertionError("failed cast moved across an effect");
                System.out.println(i + ":cast:" + InnerHost.trace);
            }
        }
        System.out.println("static=" + Modifier.isStatic(InnerHost.Member.class.getModifiers()));
    }
}
