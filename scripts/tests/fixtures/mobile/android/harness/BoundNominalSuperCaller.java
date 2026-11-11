import fixture.Core;
public final class BoundNominalSuperCaller {
    public static Core made(String a, String b, int x, int y, long tag) {
        return new Core(a, b, x, y, tag);
    }
    public static Core direct(String a, String b, Integer x, Integer y, long tag) {
        return new Core(a, b, x, y, tag);
    }
}
