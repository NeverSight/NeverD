package fixture;

@SuppressWarnings("unchecked")
public interface Api {
    Box<String> Z_FIRST = Provider.first();
    Box<Box<? extends CharSequence>> A_SECOND = Provider.next(Trace.first);
    Box<Api> SELF = Provider.self(Trace.second);
}
