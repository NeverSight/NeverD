import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ArrayCarrierSuperclassHarness {
    static int values,identities,states,failures;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a,Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route,int kind,Core self,Token token,Object item,int index,long wide) throws Throwable {
        String name=kind==0?"write":kind==1?"count":"read";
        Class<?>[] params=kind==0?new Class<?>[]{Token.class,Object.class,int.class,long.class}:new Class<?>[]{Object.class};
        Object[] args=kind==0?new Object[]{token,item,index,wide}:new Object[]{item};
        try {
            if (route==0) {
                if (kind==0) { self.write(token,item,index,wide); return null; }
                if (kind==1) return self.count(item); return self.read(item);
            }
            if (route==1) {
                Parent parent=self;
                if (kind==0) { parent.send(token,item,index,wide); return null; }
                if (kind==1) return parent.measure(item); return parent.receive(item);
            }
            if (route==2) return Core.class.getMethod(name,params).invoke(self,args);
            if (route==3) {
                MethodHandle handle=MethodHandles.publicLookup().unreflect(Core.class.getMethod(name,params));
                if (kind==0) { handle.invoke(self,token,item,index,wide); return null; }
                if (kind==1) return (int)handle.invoke(self,item); return handle.invoke(self,item);
            }
            if (route==4) return ArrayCarrierSuperclassCaller.call(kind,self,token,item,index,wide);
            if (kind==0) { Caller.write(self,token,item,index,wide); return null; }
            if (kind==1) return Caller.count(self,item); return Caller.read(self,item);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        Type[] types=((ParameterizedType)Core.class.getGenericSuperclass()).getActualTypeArguments();
        value(types[0]==Carrier.class && types[1]==Result.class);
        for (Method m:Core.class.getDeclaredMethods()) if ((m.getName().equals("write")||m.getName().equals("count")||m.getName().equals("read"))
                && java.util.Arrays.asList(m.getParameterTypes()).contains(Object.class)) value(m.isBridge()&&m.isSynthetic());
        byte[][] arrays={null,new byte[0],new byte[]{Byte.MIN_VALUE,0,Byte.MAX_VALUE}};
        RuntimeException early=new IllegalStateException("owned unbox failure"), late=new IllegalArgumentException("owned delegate failure");
        for (int route=0;route<6;route++) for (int kind=0;kind<3;kind++) for (byte[] data:arrays)
            for (int failed=0;failed<3;failed++) {
                Core self=new Core(); Carrier item=new Carrier(data); Token token=new Token(); int[] marker={0};
                self.marker=item.marker=marker; self.result= data==null?null:new Result(data);
                item.failure=failed==1?early:null; self.failure=failed==2?late:null;
                Object result=null; Throwable error=null; int index=Integer.MIN_VALUE+kind; long wide=Long.MIN_VALUE+route;
                try { result=call(route,kind,self,token,item,index,wide); } catch (Throwable actual) { error=actual; }
                state(item.calls==1 && self.calls==(failed==1?0:1) && marker[0]==(failed==1?3:8));
                if (failed!=0) { same(error,failed==1?early:late); failures++; }
                else {
                    same(error,null); same(self.observed,data);
                    if (kind==0) { same(token.receiver,self); same(token.accepted,data); value(token.index==index&&token.wide==wide); }
                    else if (kind==1) value(((Integer)result)==(data==null?-1:data.length));
                    else { same(result,self.result); if (result!=null) same(((Result)result).value,data); }
                    if (data!=null&&data.length!=0) { self.observed[0]=19; value(data[0]==19); }
                }
                if (failed!=0) same(token.receiver,null);
                value(true);
            }
        for (int route=0;route<6;route++) for (int kind=0;kind<3;kind++) for (int fault=0;fault<5;fault++) {
            Core self=new Core(); Carrier item=new Carrier(new byte[]{1}); self.result=new Result(item.value); Token token=new Token();
            Object input=(fault==0||fault==4)?new Object():fault==1?null:item;
            if (fault==3) { item.failure=early; token=null; }
            Throwable error=null;
            try { call(route,kind,(fault==2||fault==4)?null:self,token,input,0,1); } catch (Throwable actual) { error=actual; }
            value(error!=null && error.getClass()==(fault==0?ClassCastException.class:fault==3?early.getClass():NullPointerException.class));
            if (fault==3) same(error,early);
            state(self.calls==0 && item.calls==(fault==3?1:0)); failures++;
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("original-array-unbox-and-delegates=retained,complete-parent-arguments=retained,typed-entries-and-forwarding-frames=reported");
    }
}
