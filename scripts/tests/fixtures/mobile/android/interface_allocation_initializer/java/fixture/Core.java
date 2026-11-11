package fixture;
public final class Core {
    public Core() {}
    public static Api get(int index) {
        if (index == 0) return Api.zFirst;
        if (index == 1) return Api.aSecond;
        return Api.mThird;
    }
}
