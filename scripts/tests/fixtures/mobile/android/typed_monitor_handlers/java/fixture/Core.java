package fixture;
public final class Core {
    private Core() {}
    public static Object call(Object lock,Callback callback,Object seed) throws Exception {
        synchronized (lock) {
                Object result;
                try {
                    result = callback.produce(lock, seed);
                } catch (Specific failure) {
                    result = callback.refine(lock, seed, failure);
                } catch (Problem failure) {
                    result = callback.recover(lock, seed, failure);
                } catch (Exception failure) {
                    callback.observe(lock, seed, failure);
                    throw failure;
                } finally {
                    callback.finish(lock, seed);
                }
                return result;
        }
    }
    public static Object nested(Object outer,Object lock,Callback callback,Object seed) throws Exception {
        synchronized (outer) {
            synchronized (lock) {
                Object result;
                try {
                    result = callback.produce(lock, seed);
                } catch (Specific failure) {
                    result = callback.refine(lock, seed, failure);
                } catch (Problem failure) {
                    result = callback.recover(lock, seed, failure);
                } catch (Exception failure) {
                    callback.observe(lock, seed, failure);
                    throw failure;
                } finally {
                    callback.finish(lock, seed);
                }
                return result;
            }
        }
    }
}
