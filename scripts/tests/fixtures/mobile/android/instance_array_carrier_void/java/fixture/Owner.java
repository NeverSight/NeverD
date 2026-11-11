package fixture;
public class Owner {
    public Object saved;
    public Owner(Provider provider, Payload[] values, Contract.Creator<Payload> creator) {
        Trace.owner = this; Trace.hit(1); provider.fill(values, creator); Trace.hit(6); saved = values;
    }
    public static Owner create(Provider provider, Payload[] values, Contract.Creator<Payload> creator) {
        return new Owner(provider, values, creator);
    }
}
