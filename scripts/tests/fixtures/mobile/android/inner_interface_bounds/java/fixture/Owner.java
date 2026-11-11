package fixture;

public final class Owner<T extends Item> {
    public T value;
    public int calls;
    public Owner(T value) { this.value = value; }
    public T outerRead() { return value; }
    public Member member(int tag) { return new Member(tag); }
    public final class Member {
        public final int tag;
        public T value;
        public Member(int tag) { this.tag = tag; this.value = Owner.this.value; }
        public T choose(T input) { ++calls; value = input; return input; }
        public Object use(T input) { ++calls; return input.get(); }
        public <T> T echo(T input) { return input; }
    }
}
