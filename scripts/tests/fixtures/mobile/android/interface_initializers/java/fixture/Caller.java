package fixture;
public final class Caller {
    public static Object read(int slot) {
        if (slot == 0) return Api.Z_FIRST;
        if (slot == 1) return Api.A_SECOND;
        return Api.COPY;
    }
}
