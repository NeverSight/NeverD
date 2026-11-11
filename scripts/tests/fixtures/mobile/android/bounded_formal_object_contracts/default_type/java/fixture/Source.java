package fixture;

public interface Source extends ObjectView, OrderNode, TypeView {
    Bound get();
    Bound echo(Callback<String, byte[]> callback);

    @Override
    default Class<? extends Bound> type() {
        return Token.class;
    }
}
