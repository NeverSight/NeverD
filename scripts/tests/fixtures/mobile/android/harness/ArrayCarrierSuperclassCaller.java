import fixture.*;
public final class ArrayCarrierSuperclassCaller {
    public static Object call(int kind,Core self,Token token,Object item,int index,long wide) {
        if (kind == 0) { self.write(token,item,index,wide); return null; }
        if (kind == 1) return self.count(item);
        return self.read(item);
    }
}
