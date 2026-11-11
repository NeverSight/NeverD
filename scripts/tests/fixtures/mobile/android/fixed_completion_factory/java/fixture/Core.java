package fixture;
public final class Core {
    private Core() {}
    public static <R> Completion<Leaf> unary(Unary<? super Completion<? super R>,? extends Object> block,Completion<? super R> completion) {
        return Factory.unary(block,completion);
    }
    public static <T,R> Completion<Leaf> seeded(Binary<? super T,? super Completion<? super R>,? extends Object> block,T receiver,Completion<? super R> completion) {
        return Factory.seeded(block,receiver,completion);
    }
}
