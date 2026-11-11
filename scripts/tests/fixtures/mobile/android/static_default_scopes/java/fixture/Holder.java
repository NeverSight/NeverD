package fixture;

public final class Holder<Payload> {
    public final Token<Payload> token;
    public final Object tag;
    public Holder(Token<Payload> token, Object tag) {
        this.token = token;
        this.tag = tag;
        State.construct(token, tag);
    }
}
