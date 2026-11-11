package fixture;
public interface Callback {
    Object produce(Object lock,Object seed) throws Exception;
    Object refine(Object lock,Object seed,Specific failure) throws Exception;
    Object recover(Object lock,Object seed,Problem failure) throws Exception;
    void observe(Object lock,Object seed,Exception failure) throws Exception;
    void finish(Object lock,Object seed) throws Exception;
}
