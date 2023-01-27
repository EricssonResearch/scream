function test_v_a(a,Tlim,I,Bmax,Cmax,Dmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;

        
    figure(1);
    subplot(3,1,1);
    ce = a(:,13);
    if (I>1)
      k = 6;
      for n=2:I
        ce = ce+a(:,13+k);k=k+6;
      end
    end  
    plot(T,a(:,6)/1e3,T,ce/1e3);
    set(gca,'FontSize',12);grid on;
    set(gca,'XTickLabel',[]);    
    axis([Tlim(1) Tlim(2) 0 Bmax(1)*1.5]);grid on;
    title('Throughput and CE rate [Mbps]');
    
    subplot(3,1,2);
    plot(T,a(:,2),T,a(:,3));
    %,T,a(:,4)); 
    set(gca,'FontSize',12);grid on;
    set(gca,'XTickLabel',[]);    
    axis([Tlim(1) Tlim(2) 0 Dmax]);grid on;
    title('qdel[s]');
    
    subplot(3,1,3);
    plot(T,a(:,4),T,a(:,5),T,a(:,7)*20000,'k');
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 Cmax]);grid on;    
    title('CWND & in flight [byte]');    
    xlabel('T [s]');

    if I>0
    figure(2);
    subplot(2,1,1);
    plot(T,a(:,8)); 
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 0.05]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay [s]');
    subplot(2,1,2);
    plot(T,a(:,9)/1000,T,a(:,11)/1000,T,a(:,10)/1000);
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 Bmax(1)]);grid on;
    title('Bitrate [Mbps]');    
    legend('Target','Transmitted', 'RTP', 'X');
    xlabel('T [s]');
    end

    if I>1
    K = 6;    
    figure(3);
    subplot(2,1,1);
    plot(T,a(:,8+K)); 
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 0.05]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,9+K)/1000,T,a(:,11+K)/1000,T,a(:,10+K)/1000);
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 Bmax(2)]);grid on;
    title('Bitrate [kbps]');    
    legend('Target','Transmitted', 'RTP');
    xlabel('T [s]');
    end

    if I>2        
    K = 12;    
    figure(4);
    subplot(2,1,1);
    plot(T,a(:,8+K)); 
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 0.05]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,9+K)/1000,T,a(:,11+K)/1000,T,a(:,10+K)/1000);
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 Bmax(3)]);grid on;
    title('Bitrate [kbps]');    
    legend('Target','Transmitted', 'RTP');
    xlabel('T [s]');
    end

    if I>3        
    K = 18;    
    figure(5);
    subplot(2,1,1);
    plot(T,a(:,8+K)); 
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 0.05]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,9+K)/1000,T,a(:,11+K)/1000,T,a(:,10+K)/1000);
    set(gca,'FontSize',12);grid on;
    axis([Tlim(1) Tlim(2) 0 Bmax(3)]);grid on;
    title('Bitrate [kbps]');    
    legend('Target','Transmitted', 'RTP');
    xlabel('T [s]');
    end
    