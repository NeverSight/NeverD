import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"unchecked", "rawtypes"})
public final class NominalResultInvocationHarness {
    static int calls, failures;
    static void check(boolean ok) { if(!ok) throw new AssertionError("nominal result contract"); }
    static final long WORD=Long.MIN_VALUE+27, NAN=0xfff0000000000001L;
    static final int INDEX=Integer.MIN_VALUE+19;
    static final double BITS=Double.longBitsToDouble(NAN);
    static final Class<?>[] PARAMS={Object.class,int.class,long.class,double.class};
    static class Derived extends Provider {
        int entries;
        public <T extends Value> Carrier<T> read(Object token,int index,long word,double bits) {
            entries++; return super.read(token,index,word,bits);
        }
    }
    static class DerivedCarrier<E> extends Carrier<E> {
        int entries;
        public E get() { entries++; return super.get(); }
    }
    static final class Other implements Value {}
    static Carrier call(int route,Provider self,Object token) throws Throwable {
        try {
            switch(route) {
            case 0:return self.read(token,INDEX,WORD,BITS);
            case 1:return ((Api)self).read(token,INDEX,WORD,BITS);
            case 2:return (Carrier)Provider.class.getMethod("read",PARAMS).invoke(self,token,INDEX,WORD,BITS);
            case 3:return (Carrier)MethodHandles.publicLookup().findVirtual(Provider.class,"read",MethodType.methodType(Carrier.class,PARAMS)).invokeExact(self,token,INDEX,WORD,BITS);
            case 4:return NominalResultInvocationCaller.direct(self,token,INDEX,WORD,BITS);
            case 5:return Caller.direct(self,token,INDEX,WORD,BITS);
            default:return Caller.viaApi(self,token,INDEX,WORD,BITS);
            }
        } catch(InvocationTargetException error) { throw error.getCause(); }
    }
    static Item checked(int route,Provider self,Object token) {
        if(route==0)return self.<Item>read(token,INDEX,WORD,BITS).get();
        if(route==1)return NominalResultInvocationCaller.checked(self,token,INDEX,WORD,BITS);
        return Caller.checked(self,token,INDEX,WORD,BITS);
    }
    static Carrier stat(int route,Object token) throws Throwable {
        try {
            switch(route) {
            case 0:return Provider.staticRead(token,INDEX,WORD,BITS);
            case 1:return NominalResultInvocationCaller.stat(token,INDEX,WORD,BITS);
            case 2:return Caller.stat(token,INDEX,WORD,BITS);
            case 3:return (Carrier)Provider.class.getMethod("staticRead",PARAMS).invoke(null,token,INDEX,WORD,BITS);
            default:return (Carrier)MethodHandles.publicLookup().findStatic(Provider.class,"staticRead",MethodType.methodType(Carrier.class,PARAMS)).invokeExact(token,INDEX,WORD,BITS);
            }
        } catch(InvocationTargetException error) { throw error.getCause(); }
    }
    static void recorded(Provider self,Object token,int count) {
        check(self.calls==count && self.seen==token && self.index==INDEX && self.stamp==WORD && Double.doubleToRawLongBits(self.fraction)==NAN);
        if(self instanceof Derived)check(((Derived)self).entries==count);
    }
    static void outcome(Throwable actual, Class<?> kind, Throwable original) {
        if(kind==null)check(actual==null);
        else { check(actual!=null && actual.getClass()==kind && (original==null || actual==original)); failures++; }
        calls++;
    }
    public static void main(String[] args) throws Throwable {
        for(Class<?> owner:new Class<?>[]{Api.class,Provider.class}) {
            Method method=owner.getMethod("read",PARAMS);
            TypeVariable<?>[] formals=method.getTypeParameters();
            check(formals.length==1 && formals[0].getBounds()[0]==Value.class && formals[0].getGenericDeclaration().equals(method));
            ParameterizedType result=(ParameterizedType)method.getGenericReturnType();
            check(result.getRawType()==Carrier.class && result.getActualTypeArguments()[0].equals(formals[0]));
            check(method.getReturnType()==Carrier.class && !method.isBridge() && !method.isSynthetic() && method.getExceptionTypes().length==0);
            for(Type p:method.getGenericParameterTypes())check(p instanceof Class<?>);
        }
        for(int route=0;route<7;route++)for(boolean derived:new boolean[]{false,true}) {
            Provider self=derived?new Derived():new Provider(); Object token=new Object(); int count=0;
            Carrier<Value> carrier=new Carrier<>(); carrier.stored=new Other();
            for(int mode=0;mode<7;mode++) {
                self.stored=mode==0?null:mode==3?new Object():carrier;
                RuntimeException original=new IllegalStateException("owned provider failure");
                self.failure=mode==4?original:null;
                Object result=null; Throwable error=null;
                try { result=call(route,mode==5?null:self,token); }catch(Throwable e){error=e;}
                Class<?> kind=mode==3?ClassCastException.class:mode==4?original.getClass():mode==5?NullPointerException.class:null;
                outcome(error,kind,mode==4?original:null);
                if(mode!=5)count++;
                recorded(self,token,count);
                if(kind==null)check(result==self.stored);
                check(carrier.reads==0);
                carrier.stored=new Object();
            }
        }
        for(int route=0;route<3;route++) {
            Provider self=new Derived(); Object token=new Object(); int count=0;
            DerivedCarrier<Item> carrier=new DerivedCarrier<>(); Item item=new Item();
            for(int mode=0;mode<8;mode++) {
                self.stored=mode==4?new Object():mode==5?null:carrier;
                carrier.stored=mode==0?null:mode==2?new Other():mode==3?new Object():item;
                carrier.reads=carrier.entries=0;
                RuntimeException original=new UnsupportedOperationException("owned checked failure");self.failure=mode==6?original:null;
                Object result=null;Throwable error=null;
                try{result=checked(route,mode==7?null:self,token);}catch(Throwable e){error=e;}
                Class<?> kind=(mode>=2 && mode<=4)?ClassCastException.class:(mode==5 || mode==7)?NullPointerException.class:mode==6?original.getClass():null;
                outcome(error,kind,mode==6?original:null);
                if(mode!=7)count++;
                recorded(self,token,count);
                int reads=mode<=3?1:0;check(carrier.reads==reads && carrier.entries==reads);
                if(kind==null)check(result==carrier.stored);
            }
        }
        for(int route=0;route<5;route++) {
            Provider.staticCalls=0; Object token=new Object();Carrier<Box> carrier=new Carrier<>();carrier.stored=new Object();
            for(int mode=0;mode<5;mode++) {
                Provider.staticStored=mode==0?null:mode==2?new Object():carrier;
                RuntimeException original=new IllegalArgumentException("owned static failure");Provider.staticFailure=mode==3?original:null;
                Object result=null;Throwable error=null;
                try{result=stat(route,token);}catch(Throwable e){error=e;}
                Class<?> kind=mode==2?ClassCastException.class:mode==3?original.getClass():null;
                outcome(error,kind,mode==3?original:null);
                check(Provider.staticCalls==mode+1 && Provider.staticSeen==token && Provider.staticIndex==INDEX && Provider.staticStamp==WORD && Double.doubleToRawLongBits(Provider.staticFraction)==NAN);
                if(kind==null)check(result==Provider.staticStored);
                check(carrier.reads==0);
            }
        }
        Provider ignored=new Derived();Object token=new Object();Carrier<Value> carrier=new Carrier<>();carrier.stored=new Object();ignored.stored=carrier;
        Caller.ignored(ignored,token,INDEX,WORD,BITS);recorded(ignored,token,1);check(carrier.reads==0);
        ignored.stored=new Object();Throwable error=null;
        try{Caller.ignored(ignored,token,INDEX,WORD,BITS);}catch(Throwable e){error=e;}
        outcome(error,ClassCastException.class,null);recorded(ignored,token,2);
        System.out.println("calls="+calls+",failures="+failures);
        System.out.println("nominal-results-original-calls-casts-and-identities=matched");
    }
}
