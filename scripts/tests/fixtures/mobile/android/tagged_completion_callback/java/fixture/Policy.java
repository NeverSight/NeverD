package fixture;
public interface Policy {
    <R> Object work(Tag tag,Callback<? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion);
}
