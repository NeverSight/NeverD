import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public class CapturedCovariantHarness {
    static int calls, failures;
    static void check(boolean ok) { if(!ok) throw new AssertionError("captured covariance contract"); }
    static Object call(int route,Base self,int index,Class<?> owner) throws Throwable {
        try {
            switch(route) {
            case 0:return Client.read(self,index);
            case 1:return CapturedCovariantCaller.read(self,index);
            case 2:return ((Api)self).getItem(index);
            case 3:return ((Root)self).getItem(index);
            case 4:
            case 6:
                for(Method method:owner.getDeclaredMethods())
                    if(method.getName().equals("getItem") && method.getReturnType()==(route==4?Object.class:Item.class)) {
                        method.setAccessible(true); return method.invoke(self,index);
                    }
                throw new AssertionError("missing original slot");
            default:return (Object)MethodHandles.publicLookup().findVirtual(Base.class,"getItem",MethodType.methodType(Object.class,int.class)).invokeExact(self,index);
            }
        } catch(InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        for(int kind=0;kind<3;kind++)for(int route=0;route<7;route++) {
            Owner owner=new Owner();Owner other=new Owner();other.items=new Item[]{new Item()};
            Base self=kind==0?owner.createPublic():kind==1?owner.createPrivate():new CapturedCovariantDerived(owner);
            Base unrelated=other.createPrivate();
            Class<?> declaring=kind==2?Owner.Worker.class:self.getClass();
            int count=0;
            for(int mode=0;mode<8;mode++) {
                Item item=new Item();item.marker=100+mode;
                owner.items=(mode==2 || mode==5)?null:new Item[]{mode==1?null:item};
                RuntimeException original=new IllegalStateException("owned captured failure");owner.failure=mode==5?original:null;
                int index=mode==3?-1:mode==4?Integer.MAX_VALUE:0;
                Object result=null;Throwable error=null;
                try { result=call(route,mode==7?null:self,index,declaring); }catch(Throwable e){error=e;}
                Class<?> expected=mode==2?NullPointerException.class:(mode==3 || mode==4)?ArrayIndexOutOfBoundsException.class:mode==5?original.getClass():mode==7?NullPointerException.class:null;
                if(mode!=7)count++;
                check(owner.calls==count && owner.lastIndex==(mode==7?0:index) && other.calls==0);
                if(self instanceof CapturedCovariantDerived)check(((CapturedCovariantDerived)self).entries==count);
                if(expected==null)check(error==null && result==owner.items[0]);
                else {check(error!=null && error.getClass()==expected && (mode!=5 || error==original));failures++;}
                calls++;
                check(unrelated!=self);
            }
        }
        System.out.println("calls="+calls+",failures="+failures+",captures-and-original-virtual-bridges=matched");
    }
}
