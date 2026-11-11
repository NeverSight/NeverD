import fixture.*;
public class CapturedCovariantCaller {
    public static Object read(Base self,int index) { return self.getItem(index); }
}
