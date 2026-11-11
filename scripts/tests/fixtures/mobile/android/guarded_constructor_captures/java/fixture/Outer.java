package fixture;

public class Outer {
    public static int getters;
    public static int constructions;
    public int limit;
    public boolean fail;
    public final IllegalStateException sentinel = new IllegalStateException("owned getter failure");
    public int limit() {
        getters++;
        if (fail) throw sentinel;
        return limit;
    }
    public final class Member {
        public Outer owner() { return Outer.this; }
        public final int first;
        public final int second;
        public final int third;
        public Member(int first, int second, int third) {
            this.first = first;
            this.second = second;
            this.third = third;
            constructions++;
        }
        public Member(int first, int second, int third, int flags, Object unused) {
            this((flags & 1) == 0 ? first : 0,
                 (flags & 2) == 0 ? second : 0,
                 (flags & 4) == 0 ? third : Outer.this.limit());
        }
    }
}
