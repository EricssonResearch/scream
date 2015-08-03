function plot_all(a,Tmax,Bmax,Cmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;
    figure(1);
    subplot(2,1,1);
    plot(T,a(:,2));%,T,a(:,4));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.2]);grid on;
    title('OWD [s]');
    
    subplot(2,1,2);
    plot(T,a(:,15),T,a(:,22),T,a(:,29));
    set(gca,'FontSize',12);grid on;
    title('RTP queue delay [s]');    
    legend('Stream 1','Stream 2','Stream 3');
    axis([0 Tmax 0 0.1]);grid on;
    xlabel('T [s]');
    
    figure(2);
    subplot(2,1,1);
    plot(T,a(:,8),T,a(:,11));
    set(gca,'FontSize',12);grid on;
    title('CWND [byte]');    
    legend('CWND','In flight');
    axis([0 Tmax 0 Cmax]);grid on;  
    
    subplot(2,1,2);
    K=100;B_ = ones(1,K)/K;
    TTT = [00 39.9 40 59.9 60 100];
    BBB = [5e6 5e6 1e6 1e6 5e6 5e6];
    plot(T,a(:,16),T,a(:,23),T,a(:,30),T,filter(B_,1,a(:,20)+a(:,27)+a(:,34)),'k');%,TTT,BBB,'r');
    set(gca,'FontSize',12);grid on;

    axis([0 Tmax 0 Bmax]);grid on;
    title('Bitrate [bps]');    
    legend('Target, stream 1','Target, stream 2','Target, stream 3','Throughput');
    xlabel('T [s]');

    
    