package fixture;
import java.util.Collection;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Iterable direct(Iterable input,Matcher matcher) {return Factory.keep(input,matcher);}
    public static Collection checked(Collection input,Matcher matcher) {return Factory.keep(input,matcher);}
    public static Object ignored(Iterable input,Matcher matcher) {Factory.keep(input,matcher);return Trace.marker;}
    public static <T extends Iterable<E>,E> T relay(T input,Matcher<E> matcher) {return Factory.keep(input,matcher);}
    public static <T extends Collection<E>,E> T narrow(T input,Matcher<E> matcher) {return Factory.keep(input,matcher);}
}
