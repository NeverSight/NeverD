package fixture;
import java.util.Collection;
import java.util.List;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Collection direct(Collection input,Collection target,Matcher matcher) {return Factory.keep(input,target,matcher);}
    public static List checked(List input,List target,Matcher matcher) {return Factory.keep(input,target,matcher);}
    public static Object ignored(Collection input,Collection target,Matcher matcher) {Factory.keep(input,target,matcher);return Trace.marker;}
    public static <T extends Collection<E>,E> T relay(T input,T target,Matcher<? super E> matcher) {return Factory.keep(input,target,matcher);}
    public static <T extends List<E>,E> T narrow(T input,T target,Matcher<? super E> matcher) {return Factory.keep(input,target,matcher);}
}
