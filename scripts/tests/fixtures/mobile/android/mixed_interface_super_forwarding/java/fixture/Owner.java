package fixture;
final class Owner extends Host.Base implements Api, SideA, SideB, SideC {
    @Override public int measure(Context context, Input input, int value) {
        return Api.super.measure(context, input, value);
    }
    @Override public long measure(Context context, Input input, long value) {
        return Api.super.measure(context, input, value);
    }
}
