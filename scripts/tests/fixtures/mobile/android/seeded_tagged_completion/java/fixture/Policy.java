package fixture;
public interface Policy {
    <T,R> Object work(T input,Tag tag,Callback<? super T,? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion);
}
